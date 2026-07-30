//! Ported from `tests/unit/test_text_util.c` — exercises the C-ABI entry
//! points (`gtkhx_text_to_utf8`, `gtkhx_text_for_wire`, the emoji toggle)
//! through their real signatures, freeing each `g_malloc`'d return.
//!
//! `to_utf8` covers three branches: already-valid UTF-8 passthrough, Mac
//! Roman decode, and the pathological-length guards. `for_wire` covers UTF-8
//! passthrough, legacy Mac Roman encode, emoji→shortcode rewrite (+ its
//! toggle), and body LF→CR. The emoji toggle is `thread_local`, so the
//! toggle-off test can't leak into the parallel emoji tests.

use super::*;
use glib::ffi::{gboolean, GFALSE, GTRUE};
use std::os::raw::c_void;

/// Call `gtkhx_text_to_utf8(ptr, len, &out_len)` and hand back the reported
/// bytes (exactly `out_len` of them, embedded NULs preserved) + the length.
/// Frees the returned buffer.
unsafe fn to_utf8_raw(ptr: *const c_char, len: usize) -> (Vec<u8>, usize) {
    let mut out_len: usize = 0xdead_beef;
    let r = gtkhx_text_to_utf8(ptr, len, &mut out_len);
    assert!(!r.is_null(), "gtkhx_text_to_utf8 must never return NULL");
    let bytes = std::slice::from_raw_parts(r as *const u8, out_len).to_vec();
    glib::ffi::g_free(r as *mut c_void);
    (bytes, out_len)
}

unsafe fn to_utf8(input: &[u8]) -> (Vec<u8>, usize) {
    to_utf8_raw(input.as_ptr() as *const c_char, input.len())
}

/// `gtkhx_text_to_utf8` with a NULL `out_len` — must still succeed; reads the
/// result via C-string length (trailing NUL).
unsafe fn to_utf8_no_len(input: &[u8]) -> Vec<u8> {
    let r = gtkhx_text_to_utf8(
        input.as_ptr() as *const c_char,
        input.len(),
        std::ptr::null_mut(),
    );
    assert!(!r.is_null());
    let s = std::ffi::CStr::from_ptr(r).to_bytes().to_vec();
    glib::ffi::g_free(r as *mut c_void);
    s
}

unsafe fn for_wire_raw(
    ptr: *const c_char,
    len: usize,
    utf8_mode: gboolean,
    is_body: gboolean,
) -> (Vec<u8>, usize) {
    let mut out_len: usize = 0xdead_beef;
    let r = gtkhx_text_for_wire(ptr, len, utf8_mode, is_body, &mut out_len);
    assert!(!r.is_null(), "gtkhx_text_for_wire must never return NULL");
    let bytes = std::slice::from_raw_parts(r as *const u8, out_len).to_vec();
    glib::ffi::g_free(r as *mut c_void);
    (bytes, out_len)
}

unsafe fn for_wire(input: &[u8], utf8_mode: bool, is_body: bool) -> (Vec<u8>, usize) {
    let m = if utf8_mode { GTRUE } else { GFALSE };
    let b = if is_body { GTRUE } else { GFALSE };
    for_wire_raw(input.as_ptr() as *const c_char, input.len(), m, b)
}

fn is_valid_utf8(b: &[u8]) -> bool {
    std::str::from_utf8(b).is_ok()
}

// ---------- Branch 1: already-valid UTF-8 ----------

#[test]
fn valid_utf8_passthrough_ascii() {
    let input = b"hello world";
    let (out, len) = unsafe { to_utf8(input) };
    assert_eq!(out, input);
    assert_eq!(len, input.len());
}

#[test]
fn valid_utf8_passthrough_multibyte() {
    // "café" — 'é' is 0xc3 0xa9, 5 bytes total.
    let input = b"caf\xc3\xa9";
    let (out, len) = unsafe { to_utf8(input) };
    assert_eq!(out, input);
    assert_eq!(len, input.len());
    assert!(is_valid_utf8(&out));
}

#[test]
fn valid_utf8_preserves_embedded_nul() {
    let input = &[b'a', b'b', 0, b'c', b'd'];
    let (out, len) = unsafe { to_utf8(input) };
    assert_eq!(len, input.len());
    assert_eq!(out, input);
}

// ---------- Branch 2: Mac Roman → UTF-8 ----------

#[test]
fn mac_roman_e_acute() {
    // "caf" + Mac Roman 0x8e (é).
    let input = &[b'c', b'a', b'f', 0x8e];
    assert!(!is_valid_utf8(input));
    let (out, _) = unsafe { to_utf8(input) };
    assert!(is_valid_utf8(&out));
    assert_eq!(out, b"caf\xc3\xa9");
}

#[test]
fn mac_roman_curly_quotes() {
    // Mac Roman 0xd2 0xd3 = open/close double quote → U+201C U+201D.
    let input = &[0xd2, b'h', b'i', 0xd3];
    assert!(!is_valid_utf8(input));
    let (out, _) = unsafe { to_utf8(input) };
    assert!(is_valid_utf8(&out));
    assert_eq!(out, b"\xe2\x80\x9chi\xe2\x80\x9d");
}

#[test]
fn high_byte_garbage_still_returns_valid_utf8() {
    // Invalid UTF-8 pile; Mac Roman maps every byte → always valid UTF-8 out.
    let input = &[0xff, 0xfe, 0xff, 0xfe, 0xc0, 0xc1];
    assert!(!is_valid_utf8(input));
    let (out, len) = unsafe { to_utf8(input) };
    assert!(is_valid_utf8(&out));
    // len_out == strlen(out): no embedded NULs from the decode.
    assert_eq!(len, out.iter().take_while(|&&b| b != 0).count());
    assert!(!out.is_empty());
}

// ---------- Edge cases on the API ----------

#[test]
fn null_input_returns_empty() {
    let (out, len) = unsafe { to_utf8_raw(std::ptr::null(), 0) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn null_input_null_out_len() {
    // NULL bytes + NULL out_len must not crash.
    let r = unsafe { gtkhx_text_to_utf8(std::ptr::null(), 0, std::ptr::null_mut()) };
    assert!(!r.is_null());
    let s = unsafe { std::ffi::CStr::from_ptr(r).to_bytes().to_vec() };
    unsafe { glib::ffi::g_free(r as *mut c_void) };
    assert_eq!(s, b"");
}

#[test]
fn empty_input_zero_length() {
    let (out, len) = unsafe { to_utf8(b"") };
    assert_eq!(len, 0);
    assert_eq!(out, b"");
}

#[test]
fn out_len_optional_on_valid_input() {
    let out = unsafe { to_utf8_no_len(b"valid") };
    assert_eq!(out, b"valid");
}

#[test]
fn out_len_optional_on_mac_roman() {
    // "a" + Mac Roman 0x8e, NULL out_len.
    let out = unsafe { to_utf8_no_len(&[b'a', 0x8e]) };
    assert!(is_valid_utf8(&out));
    assert_eq!(out, b"a\xc3\xa9");
}

// ---------- Pathological-length guard ----------

#[test]
fn len_above_max_returns_empty() {
    // Tiny buffer + huge len: the guard must short-circuit before reading.
    let input = [b'a'];
    let huge = TO_UTF8_MAX_LEN + 1;
    let (out, len) = unsafe { to_utf8_raw(input.as_ptr() as *const c_char, huge) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn len_usize_max_returns_empty() {
    let input = [b'a'];
    let (out, len) = unsafe { to_utf8_raw(input.as_ptr() as *const c_char, usize::MAX) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn len_at_isize_max_plus_one_returns_empty() {
    // The gssize-wraparound threshold for the old C g_utf8_validate cast.
    let input = [b'a'];
    let n = (isize::MAX as usize) + 1;
    let (out, len) = unsafe { to_utf8_raw(input.as_ptr() as *const c_char, n) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn len_above_decoded_isize_cap_returns_empty() {
    // Just above the (isize::MAX - 1)/3 bound but below isize::MAX — the gap
    // where len*3 would overflow. The tight cap rejects it.
    let input = [b'a'];
    let bad_len = TO_UTF8_MAX_LEN + 1;
    assert!(bad_len <= isize::MAX as usize);
    assert!(bad_len > (isize::MAX as usize) / 3);
    let (out, len) = unsafe { to_utf8_raw(input.as_ptr() as *const c_char, bad_len) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn small_input_still_works_below_max() {
    // Guards above the bound don't reject reasonable input.
    let input = b"still works";
    let (out, len) = unsafe { to_utf8(input) };
    assert_eq!(out, input);
    assert_eq!(len, input.len());
}

// ---------- gtkhx_text_for_wire: UTF-8 mode ----------

#[test]
fn for_wire_utf8_mode_passthrough_ascii() {
    let (out, len) = unsafe { for_wire(b"hello", true, false) };
    assert_eq!(out, b"hello");
    assert_eq!(len, 5);
}

#[test]
fn for_wire_utf8_mode_passthrough_multibyte() {
    let input = b"caf\xc3\xa9";
    let (out, len) = unsafe { for_wire(input, true, false) };
    assert_eq!(len, 5);
    assert_eq!(out, input);
}

#[test]
fn for_wire_utf8_mode_keeps_lf() {
    let (out, _) = unsafe { for_wire(b"line1\nline2", true, true) };
    assert_eq!(out, b"line1\nline2");
    assert!(!out.contains(&b'\r'));
}

// ---------- for_wire: legacy mode ----------

#[test]
fn for_wire_legacy_ascii_passthrough() {
    let (out, len) = unsafe { for_wire(b"hello", false, false) };
    assert_eq!(out, b"hello");
    assert_eq!(len, 5);
}

#[test]
fn for_wire_legacy_e_acute_round_trips() {
    // "café" → 'c' 'a' 'f' 0x8e.
    let (out, len) = unsafe { for_wire(b"caf\xc3\xa9", false, false) };
    assert_eq!(len, 4);
    assert_eq!(out, &[b'c', b'a', b'f', 0x8e]);
}

#[test]
fn for_wire_legacy_curly_quotes() {
    // U+201C / U+201D → Mac Roman 0xd2 / 0xd3.
    let (out, len) = unsafe { for_wire(b"\xe2\x80\x9chi\xe2\x80\x9d", false, false) };
    assert_eq!(len, 4);
    assert_eq!(out, &[0xd2, b'h', b'i', 0xd3]);
}

#[test]
fn for_wire_legacy_emoji_to_shortcode() {
    // "ok😊" → "ok:blush:", rewritten before Mac Roman so no '?' / high bytes.
    let (out, _) = unsafe { for_wire(b"ok\xf0\x9f\x98\x8a", false, false) };
    assert_eq!(out, b"ok:blush:");
    assert!(!out.contains(&b'?'));
    assert!(out.iter().all(|&b| b < 0x80));
}

#[test]
fn for_wire_legacy_non_emoji_unmappable_still_substitutes() {
    // U+0950 (Devanagari Om): no shortcode, not in Mac Roman → '?'.
    let (out, _) = unsafe { for_wire(b"om\xe0\xa5\x90", false, false) };
    assert_eq!(out[0], b'o');
    assert_eq!(out[1], b'm');
    assert_eq!(out[2], b'?');
}

#[test]
fn for_wire_utf8_mode_keeps_emoji() {
    // UTF-8 mode must NOT rewrite emoji.
    let input = b"ok\xf0\x9f\x98\x8a";
    let (out, _) = unsafe { for_wire(input, true, false) };
    assert_eq!(out, input);
}

#[test]
fn for_wire_legacy_emoji_with_body_crlf() {
    // "hi 🎉\nbye" → "hi :tada:\rbye" (shortcode + LF→CR).
    let (out, _) = unsafe { for_wire(b"hi \xf0\x9f\x8e\x89\nbye", false, true) };
    assert_eq!(out, b"hi :tada:\rbye");
}

#[test]
fn for_wire_legacy_body_lf_to_cr() {
    let input = b"line1\nline2\nline3";
    let (out, len) = unsafe { for_wire(input, false, true) };
    assert_eq!(len, input.len());
    assert!(!out.contains(&b'\n'));
    assert_eq!(out.iter().filter(|&&b| b == b'\r').count(), 2);
}

#[test]
fn for_wire_legacy_name_keeps_lf() {
    // !is_body: LF untouched.
    let (out, _) = unsafe { for_wire(b"weird\nname", false, false) };
    assert!(out.contains(&b'\n'));
    assert!(!out.contains(&b'\r'));
}

#[test]
fn for_wire_null_input() {
    let (out, len) = unsafe { for_wire_raw(std::ptr::null(), 0, GTRUE, GTRUE) };
    assert_eq!(len, 0);
    assert_eq!(out, b"");
}

// ---------- for_wire pathological-length guard ----------
//
// The guard must fire BEFORE from_raw_parts (whose precondition is
// len <= isize::MAX). A tiny buffer + huge len exercises the contract: with
// the guard missing, slice construction is UB and later reads/allocs walk off
// the buffer. Both modes must be protected — the UTF-8 pass-through branch has
// no other length check.

#[test]
fn for_wire_len_above_max_returns_empty_utf8_mode() {
    // utf8_mode: previously the unguarded branch — this is the regression pin.
    let input = [b'a'];
    let huge = FOR_WIRE_MAX_LEN + 1;
    let (out, len) = unsafe { for_wire_raw(input.as_ptr() as *const c_char, huge, GTRUE, GFALSE) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn for_wire_len_above_max_returns_empty_legacy_mode() {
    let input = [b'a'];
    let huge = FOR_WIRE_MAX_LEN + 1;
    let (out, len) = unsafe { for_wire_raw(input.as_ptr() as *const c_char, huge, GFALSE, GFALSE) };
    assert_eq!(out, b"");
    assert_eq!(len, 0);
}

#[test]
fn for_wire_len_usize_max_returns_empty() {
    // The isize-wraparound extreme, both modes.
    let input = [b'a'];
    for mode in [GTRUE, GFALSE] {
        let (out, len) =
            unsafe { for_wire_raw(input.as_ptr() as *const c_char, usize::MAX, mode, GTRUE) };
        assert_eq!(out, b"");
        assert_eq!(len, 0);
    }
}

// ---------- emoji-shortcode toggle (thread_local: no cross-test race) ----------

#[test]
fn for_wire_legacy_emoji_toggle_off() {
    // Default is ON; turn it off for this thread only, verify the fallback '?'
    // path, then restore. thread_local isolation means concurrent emoji tests
    // on other threads still see the default ON.
    assert_eq!(gtkhx_text_emoji_shortcodes_enabled(), GTRUE);
    gtkhx_text_set_emoji_shortcodes_enabled(GFALSE);
    assert_eq!(gtkhx_text_emoji_shortcodes_enabled(), GFALSE);

    let (out, _) = unsafe { for_wire(b"ok\xf0\x9f\x98\x8a", false, false) };
    assert_eq!(out[0], b'o');
    assert_eq!(out[1], b'k');
    assert_eq!(out[2], b'?');

    gtkhx_text_set_emoji_shortcodes_enabled(GTRUE);
    assert_eq!(gtkhx_text_emoji_shortcodes_enabled(), GTRUE);
}
