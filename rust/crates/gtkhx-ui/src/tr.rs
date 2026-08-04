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

/// Translate `msgid` (which carries a single `%s` or positional `%1$s`
/// placeholder) and substitute `a` for that placeholder.
///
/// This keeps the original single-sentence msgid intact instead of
/// concatenating separately-translated fragments — so the existing catalog
/// entry still resolves and translators can position the argument naturally
/// for their language's word order. `%1$s` is tried before `%s` so a
/// positional catalog string wins; if neither placeholder is present the
/// translation is returned unchanged.
pub fn tr1(msgid: &str, a: &str) -> String {
    let s = tr(msgid);
    for pat in ["%1$s", "%s"] {
        if let Some(idx) = s.find(pat) {
            let mut out = String::with_capacity(s.len() + a.len());
            out.push_str(&s[..idx]);
            out.push_str(a);
            out.push_str(&s[idx + pat.len()..]);
            return out;
        }
    }
    s
}

/// Translate `msgid` and substitute positional `%1$s`, `%2$s`, … arguments
/// in order. Like [`tr1`] but for msgids with more than one placeholder;
/// keeping them positional lets a translation reorder the arguments.
pub fn tr_fmt(msgid: &str, args: &[&str]) -> String {
    let mut s = tr(msgid);
    for (i, a) in args.iter().enumerate() {
        s = s.replace(&format!("%{}$s", i + 1), a);
    }
    s
}

/// Translate `msgid` and substitute an ordered list of already-stringified
/// arguments for its printf-style placeholders. Handles both **sequential**
/// (`%s`, `%u`, …) and gettext **positional** (`%1$s`, `%2$u`, …) forms,
/// `%%` literals, and repeated positional references — so a translation may
/// switch to positional specifiers or reorder/repeat arguments without the
/// UI showing raw `%…` tokens. The conversion letter is ignored (every value
/// is inserted verbatim), so string and numeric arguments share one path.
/// Sequential placeholders consume `args` left to right; positional ones
/// index directly. A placeholder with no matching argument is dropped.
pub fn tr_argv(msgid: &str, args: &[&str]) -> String {
    subst(&tr(msgid), args)
}

/// Substitute `args` into an **already translated** string.
///
/// The body of [`tr_argv`], split out so the plural path can reach it:
/// `trn` has already been through the catalog, and running it through
/// `tr_argv` would translate it a second time — usually a no-op, but a
/// lookup of a translated string is not something to rely on.
fn subst(s: &str, args: &[&str]) -> String {
    let mut out = String::with_capacity(s.len() + 16);
    let mut chars = s.chars().peekable();
    let mut seq = 0usize;
    while let Some(c) = chars.next() {
        if c != '%' {
            out.push(c);
            continue;
        }
        match chars.peek().copied() {
            Some('%') => {
                chars.next();
                out.push('%');
            }
            // positional %N$<letter>
            Some(d) if d.is_ascii_digit() => {
                let mut num = String::new();
                while let Some(&d) = chars.peek() {
                    if d.is_ascii_digit() {
                        num.push(d);
                        chars.next();
                    } else {
                        break;
                    }
                }
                if chars.peek() == Some(&'$') {
                    chars.next(); // '$'
                    chars.next(); // conversion letter
                    if let Ok(n) = num.parse::<usize>() {
                        if (1..=args.len()).contains(&n) {
                            out.push_str(args[n - 1]);
                        }
                    }
                } else {
                    // Not a positional spec — emit the '%' + digits verbatim.
                    out.push('%');
                    out.push_str(&num);
                }
            }
            // sequential %<letter>
            Some(d) if d.is_ascii_alphabetic() => {
                chars.next(); // conversion letter
                if seq < args.len() {
                    out.push_str(args[seq]);
                    seq += 1;
                }
            }
            // lone '%' at end / before punctuation — emit verbatim.
            _ => out.push('%'),
        }
    }
    out
}

/// Mark a literal for extraction without translating it here.
///
/// gettext's `N_()` idiom. xgettext can only see a string where it is written,
/// so a table of `&'static str` that is translated later — the Settings page
/// list, whose titles go through `tr(entry.title)` at build time — offers it
/// nothing to extract, and every one of those strings silently never reaches a
/// translator. Wrapping the literal makes it visible to the catalog while
/// leaving it untouched at runtime; the deferred `tr` still does the work.
pub const fn n_(s: &'static str) -> &'static str {
    s
}

/// Translate `msgid` in the disambiguating context `ctx`.
///
/// gettext's `pgettext`. Some English words carry unrelated senses that no
/// other language can collapse the same way — "Login" is both the
/// account-name field and the name of a sound event, "General" is both a
/// preferences page and a server category. A context makes each sense its
/// own catalog entry, so a translator can render them differently.
///
/// There is no `pgettext` symbol to link against: GNU gettext implements it
/// as a macro that concatenates `ctx`, `U+0004` and the msgid into one
/// lookup key. We do the same. When the catalog has no entry, `dgettext`
/// hands the key straight back, so check for that and fall back to the bare
/// msgid rather than showing the user a string with a control character in
/// the middle of it.
///
/// xgettext is told about this by `--keyword=trc:1c,2` in `po/meson.build`.
pub fn trc(ctx: &str, s: &str) -> String {
    let key = format!("{ctx}\u{4}{s}");
    let Ok(c) = CString::new(key.as_str()) else {
        return s.to_owned();
    };
    unsafe {
        let p = dgettext(DOMAIN.as_ptr() as *const c_char, c.as_ptr());
        if p.is_null() {
            return s.to_owned();
        }
        let out = CStr::from_ptr(p);
        if out.to_bytes() == key.as_bytes() {
            return s.to_owned();
        }
        out.to_string_lossy().into_owned()
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

/// [`trn`] followed by [`tr_argv`]-style argument substitution.
///
/// The plural counterpart of `tr_argv`, for the common case of a plural
/// msgid whose only placeholder is the count itself. Picking the form and
/// filling in the number are one step at every call site, which is what
/// keeps callers from reaching for the `file(s)` dodge.
///
/// xgettext is told about this by `--keyword=trn_argv:1,2` in
/// `po/meson.build`.
pub fn trn_argv(singular: &str, plural: &str, n: u64, args: &[&str]) -> String {
    subst(&trn(singular, plural, n), args)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The context separator is a wire format, not an implementation
    /// detail. `msgfmt` writes a contexted entry into the `.mo` under the
    /// key `ctx \u{4} msgid`, and `trc` has to build byte-identical keys or
    /// every lookup silently misses and falls back to English. Verified
    /// against a real catalog: `po/de.po`'s `msgctxt "sound event"` /
    /// `msgid "Login"` entry lands in `gtkhx.mo` as the bytes
    /// `sound event\x04Login\0`.
    #[test]
    fn trc_key_uses_the_eot_separator() {
        let key = format!("{}\u{4}{}", "sound event", "Login");
        assert_eq!(key.as_bytes(), b"sound event\x04Login");
    }

    /// With no catalog loaded, `dgettext` hands the lookup key straight
    /// back. Returning it would show the user the context and a control
    /// character; `trc` must strip back to the bare msgid instead.
    #[test]
    fn trc_falls_back_to_the_bare_msgid() {
        let out = trc("no such context", "Untranslated Sentinel");
        assert_eq!(out, "Untranslated Sentinel");
        assert!(!out.contains('\u{4}'));
    }

    /// `subst` is shared by `tr_argv` and `trn_argv`; the plural path must
    /// not re-translate, but it must substitute identically.
    #[test]
    fn subst_handles_sequential_and_positional() {
        assert_eq!(subst("Wrote %u files.", &["3"]), "Wrote 3 files.");
        assert_eq!(subst("%2$s then %1$s", &["a", "b"]), "b then a");
        assert_eq!(subst("100%% done, %s", &["ok"]), "100% done, ok");
    }
}
