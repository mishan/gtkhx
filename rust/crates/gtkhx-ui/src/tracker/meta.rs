//! The tracker window's reading of v3 metadata. The struct itself, its
//! vocabularies and its C ABI live in `gtkhx-core::boxed::tracker`, where
//! it is built from hxproto's decoder.

pub use gtkhx_core::boxed::tracker::*;

/// Build the compact "Caps" column string from a meta pointer. Mirrors
/// the C `format_caps_badges`: `★` (promoted), `HOPE`, `TLS`, `v6`,
/// space-separated in that fixed order. Empty string when `m` is NULL
/// or advertises nothing.
///
/// # Safety
/// `m` is NULL or a valid `HxTrackerV3Meta*`.
pub unsafe fn caps_badges(m: *const HxTrackerV3Meta) -> String {
    if m.is_null() {
        return String::new();
    }
    let m = &*m;
    let mut out = String::new();
    fn push(out: &mut String, s: &str) {
        if !out.is_empty() {
            out.push(' ');
        }
        out.push_str(s);
    }
    if m.is_promoted != 0 {
        out.push('\u{2605}'); // ★ BLACK STAR
    }
    if m.supports_hope != 0 {
        push(&mut out, "HOPE");
    }
    if m.supports_tls != 0 {
        push(&mut out, "TLS");
    }
    if m.supports_ipv6 != 0 {
        push(&mut out, "v6");
    }
    out
}
