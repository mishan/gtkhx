//! Minimal gettext shim so ported windows keep their translations.
//!
//! The C side binds the `gtkhx` text domain at startup
//! (`i18n.gettext('gtkhx')` in meson, `bindtextdomain` in `main`); we're
//! in the same process, so `dgettext`/`dngettext` against that domain
//! resolve the same `.mo` catalog the C `_()` macro used. glibc provides
//! both symbols, so no extra link surface. R6's `main()` port will decide
//! whether to move to `gettext-rs`; until then this keeps the French
//! translation working for the tracker window.

use std::ffi::{c_char, CStr, CString};
use std::os::raw::c_ulong;

const DOMAIN: &[u8] = b"gtkhx\0";

extern "C" {
    fn dgettext(domain: *const c_char, msgid: *const c_char) -> *mut c_char;
    fn dngettext(
        domain: *const c_char,
        msgid: *const c_char,
        msgid_plural: *const c_char,
        n: c_ulong,
    ) -> *mut c_char;
}

/// Translate `s` via the `gtkhx` domain. Falls back to `s` verbatim on
/// any failure (embedded NUL, missing catalog). The returned pointer is
/// owned by gettext — we copy it out and never free it.
pub fn tr(s: &str) -> String {
    let Ok(c) = CString::new(s) else {
        return s.to_owned();
    };
    unsafe {
        let p = dgettext(DOMAIN.as_ptr() as *const c_char, c.as_ptr());
        if p.is_null() {
            return s.to_owned();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// Plural-aware translate: `singular` for n == 1, `plural` otherwise
/// (subject to the catalog's plural rule).
pub fn trn(singular: &str, plural: &str, n: u64) -> String {
    let (Ok(cs), Ok(cp)) = (CString::new(singular), CString::new(plural)) else {
        return if n == 1 { singular } else { plural }.to_owned();
    };
    unsafe {
        let p = dngettext(
            DOMAIN.as_ptr() as *const c_char,
            cs.as_ptr(),
            cp.as_ptr(),
            n as c_ulong,
        );
        if p.is_null() {
            return if n == 1 { singular } else { plural }.to_owned();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}
