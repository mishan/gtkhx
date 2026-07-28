//! URL autodetection, delegated to the existing C detector.
//!
//! **Deliberately not reimplemented in Rust.** `src/gtkurl.c` already
//! owns the canonical scheme list (`url_schemes[]` / `url_bare_prefixes[]`)
//! and the trailing-punctuation trimming, and its own header says both
//! the xtext path and the GtkTextView path key off that one list so
//! "adding a new scheme is a one-line edit". Writing a second detector
//! here would guarantee the two chat backends eventually disagree about
//! what a link is — which is precisely the divergence the coexistence
//! A/B exists to rule out.
//!
//! So this is a thin bridge: hand `gtkurl_scan` the parsed text, take
//! the byte ranges it reports, and mark them on the `ParsedText`.

use gtk4::glib::translate::ToGlibPtr;
use gtk4::prelude::*;
use hxchat_layout::ParsedText;
use std::ffi::{c_char, c_int, c_void, CStr, CString};

extern "C" {
    /// Scan UTF-8 `text`, invoking `cb` once per detected URL.
    fn gtkurl_scan(
        text: *const c_char,
        length: isize,
        cb: Option<unsafe extern "C" fn(*const c_char, c_int, c_int, *mut c_void)>,
        user: *mut c_void,
    );
    /// Prepend a scheme to bare `www.`-style tokens so the launcher can
    /// actually open them. Caller frees with `g_free`.
    fn gtkurl_normalize(word: *const c_char) -> *mut c_char;
    /// The Adwaita right-click popover: URL header, "Open Link in
    /// Browser", "Copy Selected Link", plus alternate browsers.
    fn gtkurl_show_popup(
        anchor: *mut gtk4::ffi::GtkWidget,
        url: *const c_char,
        x: f64,
        y: f64,
    );
    /// `gtkurl_is_url` — does this whitespace-delimited word look like
    /// a link? The same classifier the word-click handler uses.
    fn gtkurl_is_url(word: *const c_char) -> i32;
    fn g_free(p: *mut c_void);
}

/// Whether `gtkurl` classifies this word as a URL.
pub fn word_is_url(word: &str) -> bool {
    let Ok(c) = CString::new(word) else {
        return false;
    };
    unsafe { gtkurl_is_url(c.as_ptr()) != 0 }
}

/// Pop the shared URL menu, reusing the one every other surface uses.
pub fn show_url_popup(anchor: &impl IsA<gtk4::Widget>, url: &str, x: f64, y: f64) {
    let Ok(c_url) = CString::new(url) else {
        return;
    };
    unsafe {
        gtkurl_show_popup(
            anchor.as_ref().to_glib_none().0,
            c_url.as_ptr(),
            x,
            y,
        );
    }
}

/// Byte ranges collected from the C scanner.
#[derive(Default)]
struct Matches(Vec<(usize, usize)>);

unsafe extern "C" fn collect(
    _text: *const c_char,
    start: c_int,
    end: c_int,
    user: *mut c_void,
) {
    if user.is_null() || start < 0 || end <= start {
        return;
    }
    let m = &mut *(user as *mut Matches);
    m.0.push((start as usize, end as usize));
}

/// Find URLs in `p.text` and mark them as links.
///
/// Ranges that don't land on char boundaries are dropped by
/// `ParsedText::add_link`, so a detector disagreeing with Rust about
/// byte offsets degrades to "no link" rather than corrupting the spans.
pub fn autolink(p: &mut ParsedText) {
    if p.text.is_empty() {
        return;
    }
    let Ok(c_text) = CString::new(p.text.as_bytes()) else {
        // An interior NUL means we can't hand the string to C at all.
        // Chat text shouldn't contain one; if it does, skip detection
        // rather than truncating at the NUL and mis-ranging everything
        // after it.
        return;
    };
    let mut matches = Matches::default();
    unsafe {
        gtkurl_scan(
            c_text.as_ptr(),
            p.text.len() as isize,
            Some(collect),
            &mut matches as *mut Matches as *mut c_void,
        );
    }
    for (start, end) in matches.0 {
        let Some(word) = p.text.get(start..end) else {
            continue;
        };
        let href = normalize(word);
        p.add_link(start..end, href);
    }
}

/// `gtkurl_normalize`, or the word unchanged if it fails.
fn normalize(word: &str) -> String {
    let Ok(c_word) = CString::new(word) else {
        return word.to_string();
    };
    unsafe {
        let raw = gtkurl_normalize(c_word.as_ptr());
        if raw.is_null() {
            return word.to_string();
        }
        let out = CStr::from_ptr(raw).to_string_lossy().into_owned();
        g_free(raw as *mut c_void);
        out
    }
}

/// Stand-ins for the C symbols, so the test binary links.
///
/// The crate is an rlib in the real build and its externs resolve
/// against the gtkhx binary, but `cargo test` produces a standalone
/// executable with no C tree behind it — the same constraint that keeps
/// `gtkhx-core` extern-free so the Tier 2 tests can link it alone.
///
/// Defining the stubs here rather than skipping the tests has a second
/// benefit: the scanner becomes deterministic, so the `autolink` glue
/// (offset handling, normalisation, span rewriting) is actually testable
/// instead of depending on whatever `gtkurl.c` decides a URL is.
#[cfg(test)]
mod c_stubs {
    use super::*;

    /// Reports the first "http://..."-looking run, ending at whitespace.
    #[no_mangle]
    unsafe extern "C" fn gtkurl_scan(
        text: *const c_char,
        length: isize,
        cb: Option<unsafe extern "C" fn(*const c_char, c_int, c_int, *mut c_void)>,
        user: *mut c_void,
    ) {
        let (Some(cb), false) = (cb, text.is_null()) else {
            return;
        };
        let bytes = std::slice::from_raw_parts(text as *const u8, length.max(0) as usize);
        let Ok(s) = std::str::from_utf8(bytes) else {
            return;
        };
        let mut from = 0usize;
        while let Some(rel) = s[from..].find("http://") {
            let start = from + rel;
            let end = s[start..]
                .find(char::is_whitespace)
                .map(|i| start + i)
                .unwrap_or(s.len());
            cb(text, start as c_int, end as c_int, user);
            from = end;
            if from >= s.len() {
                break;
            }
        }
    }

    #[no_mangle]
    unsafe extern "C" fn gtkurl_normalize(word: *const c_char) -> *mut c_char {
        // Mirrors the real one's contract: returns a g_malloc'd copy the
        // caller frees. Plain libc malloc is fine here because the stub
        // g_free below is the matching free.
        let s = std::ffi::CStr::from_ptr(word).to_bytes();
        let buf = libc_malloc(s.len() + 1);
        std::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, s.len());
        *(buf as *mut u8).add(s.len()) = 0;
        buf as *mut c_char
    }

    #[no_mangle]
    unsafe extern "C" fn gtkurl_show_popup(
        _anchor: *mut gtk4::ffi::GtkWidget,
        _url: *const c_char,
        _x: f64,
        _y: f64,
    ) {
    }

    #[no_mangle]
    unsafe extern "C" fn gtkurl_is_url(word: *const c_char) -> i32 {
        let s = std::ffi::CStr::from_ptr(word).to_string_lossy();
        i32::from(s.starts_with("http://") || s.starts_with("https://"))
    }

    #[no_mangle]
    unsafe extern "C" fn g_free(p: *mut c_void) {
        if !p.is_null() {
            libc_free(p);
        }
    }

    extern "C" {
        #[link_name = "malloc"]
        fn libc_malloc(n: usize) -> *mut c_void;
        #[link_name = "free"]
        fn libc_free(p: *mut c_void);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn autolink_marks_a_url() {
        let mut p = ParsedText::plain("see http://example.com/x now");
        autolink(&mut p);
        assert_eq!(p.links.len(), 1);
        assert_eq!(p.links[0].href, "http://example.com/x");
        assert_eq!(&p.text[p.links[0].range.clone()], "http://example.com/x");
        assert_eq!(p.style_at(4).link, Some(0));
        assert_eq!(p.style_at(0).link, None);
        p.debug_assert_well_formed();
    }

    #[test]
    fn autolink_handles_several() {
        let mut p = ParsedText::plain("http://a.example and http://b.example");
        autolink(&mut p);
        assert_eq!(p.links.len(), 2);
        p.debug_assert_well_formed();
    }

    #[test]
    fn autolink_leaves_plain_text_alone() {
        let mut p = ParsedText::plain("nothing to see here");
        autolink(&mut p);
        assert!(p.links.is_empty());
        assert!(p.spans.is_empty());
    }

    #[test]
    fn autolink_survives_multibyte_text() {
        // The detector works in bytes; a range that isn't on a char
        // boundary must be dropped, not applied.
        let mut p = ParsedText::plain("héllo → http://example.com/ø done");
        autolink(&mut p);
        p.debug_assert_well_formed();
        assert_eq!(p.links.len(), 1);
    }

    #[test]
    fn autolink_on_empty_text_is_a_noop() {
        let mut p = ParsedText::plain("");
        autolink(&mut p);
        assert!(p.links.is_empty());
    }
}
