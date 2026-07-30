//! Populate a files-browser `GListStore` from a Hotline `HTLS_DATA_FILE_LIST`
//! reply — the full wire→model binding, in Rust.
//!
//! This replaces the C `files_remote_provider.c::populate_from_chunks` +
//! `populate_from_chunks_cb` (and the `filelist_walker.c` shim they drove).
//! Given the accumulated FILE_LIST reply bytes, it walks each entry via the
//! `hotline-proto` parser, decodes the display name (Mac Roman → UTF-8), the
//! dir flag, the icon id, and the kind label, builds an [`HxFileEntry`], and
//! appends it to the provider's `gio::ListStore`. The store is cleared first,
//! so one call fully refreshes the listing.
//!
//! Everything here is pure Rust over `gio` (no GTK, no display), so the whole
//! decode is unit-tested headless against hand-built wire buffers — coverage
//! the C callback path never had on its own.

use std::ffi::{c_char, CStr, CString};
use std::slice;

use glib::translate::from_glib_none;

use crate::files_entry::HxFileEntry;
use hotline_proto::parse::FTYPE_FLDR;

// ---- gettext shim (mirrors gtkhx-ui::tr, the `gtkhx` text domain) ---------
//
// The kind labels + the "%s file" fallback go through the same `gtkhx`
// catalog the C `_()` macro resolved against, so the ported path keeps the
// existing (French) translations. glibc provides dgettext, so no extra link
// surface and `cargo test` stays headless.

const DOMAIN: &[u8] = b"gtkhx\0";

extern "C" {
    fn dgettext(domain: *const c_char, msgid: *const c_char) -> *mut c_char;
}

/// Translate `s` via the `gtkhx` domain, falling back to `s` verbatim.
fn tr(s: &str) -> String {
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

/// Human kind label for a big-endian FourCC, mirroring
/// `files.c::kind_of_ftype`: a known type's English label run through the
/// catalog; otherwise `"XXXX file"` with non-printable bytes shown as `?`.
fn kind_for(ftype_be: [u8; 4]) -> String {
    match crate::files::kind_label_for(Some(&ftype_be)) {
        // Static ASCII English label (e.g. "MP3 Audio") → translate.
        Some(label) => tr(label.to_str().unwrap_or("")),
        None => {
            let mut safe = String::with_capacity(4);
            for &c in &ftype_be {
                safe.push(if (0x20..0x7f).contains(&c) {
                    c as char
                } else {
                    '?'
                });
            }
            // "%s file" is the catalog msgid; substitute the sanitized FourCC.
            let translated = tr("%s file");
            match translated.find("%s") {
                Some(idx) => {
                    let mut out = String::with_capacity(translated.len() + safe.len());
                    out.push_str(&translated[..idx]);
                    out.push_str(&safe);
                    out.push_str(&translated[idx + 2..]);
                    out
                }
                None => translated,
            }
        }
    }
}

/// Append the entries parsed from `data` to `store` (already cleared by the
/// caller). Split out from the FFI shell so the unit tests can drive it with
/// a real `gio::ListStore` and no raw pointers.
fn fill(store: &gio::ListStore, data: &[u8]) {
    let mut off = 0usize;
    while let Some((entry, next)) = hotline_proto::parse::parse_file_list_entry(data, off) {
        let ftype_be = entry.ftype.to_be_bytes();
        let is_dir = entry.ftype == FTYPE_FLDR;
        // Display name: Mac Roman (or already-UTF-8) wire bytes → UTF-8.
        let name = hotline_proto::text::to_utf8(entry.name);
        // Icon + kind both key off the raw big-endian FourCC; the icon also
        // consults the raw (pre-UTF-8) name bytes for "DROP BOX" / "UPLOAD".
        let icon = crate::files::icon_id_for(Some(&ftype_be), Some(entry.name));
        let kind = kind_for(ftype_be);
        // Folders carry a child count in fsize (rendered "(N items)"); files
        // carry a byte count. No mtime on the wire (0).
        let obj = HxFileEntry::build(&name, is_dir, entry.fsize as u64, 0, &kind, icon);
        store.append(&obj);
        off = next;
    }
}

/// Clear `store` and repopulate it from the FILE_LIST reply bytes `fh`
/// (`fhlen` bytes). NULL / empty `fh` just clears. NULL `store` is a no-op.
///
/// # Safety
/// `store`, when non-null, must be a live `GListStore` whose item type is
/// `HxFileEntry` (as `g_list_store_new (HX_TYPE_FILE_ENTRY)` produces).
/// `fh`, when non-null, must point to `fhlen` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_populate_from_reply(
    store: *mut gio::ffi::GListStore,
    fh: *const u8,
    fhlen: usize,
) {
    if store.is_null() {
        return;
    }
    let store: gio::ListStore = from_glib_none(store);
    store.remove_all();
    if fh.is_null() || fhlen == 0 {
        return;
    }
    // Guard the documented `from_raw_parts` ceiling (a length past isize::MAX
    // is instant UB); real replies are tiny.
    if fhlen > isize::MAX as usize {
        return;
    }
    let data = slice::from_raw_parts(fh, fhlen);
    fill(&store, data);
}

#[cfg(test)]
mod tests {
    use super::*;
    // gio::prelude re-exports the glib prelude (StaticType / Cast), so it
    // covers the downcast + static_type used below.
    use gio::prelude::*;
    use glib::subclass::prelude::*;

    /// Build one FILE_LIST chunk: 2-byte type (0xc8), 2-byte rest-len,
    /// then ftype/fcreator/fsize/unknown/fnlen (u32 BE each) + name bytes.
    fn chunk(ftype: &[u8; 4], fsize: u32, name: &[u8]) -> Vec<u8> {
        let mut c = Vec::new();
        c.extend_from_slice(&0x00c8u16.to_be_bytes()); // HTLS_DATA_FILE_LIST
        let rest_len = (20 + name.len()) as u16;
        c.extend_from_slice(&rest_len.to_be_bytes());
        c.extend_from_slice(ftype);
        c.extend_from_slice(b"MACR"); // fcreator (ignored)
        c.extend_from_slice(&fsize.to_be_bytes());
        c.extend_from_slice(&0u32.to_be_bytes()); // unknown
        c.extend_from_slice(&(name.len() as u32).to_be_bytes());
        c.extend_from_slice(name);
        c
    }

    fn make_store() -> gio::ListStore {
        gio::ListStore::with_type(HxFileEntry::static_type())
    }

    #[test]
    fn populates_folder_and_file() {
        let mut buf = Vec::new();
        buf.extend_from_slice(&chunk(b"fldr", 3, b"Uploads"));
        buf.extend_from_slice(&chunk(b"TEXT", 12, b"readme.txt"));

        let store = make_store();
        fill(&store, &buf);

        assert_eq!(store.n_items(), 2);

        let a = store.item(0).unwrap().downcast::<HxFileEntry>().unwrap();
        assert_eq!(a.imp().name.borrow().to_str().unwrap(), "Uploads");
        assert!(a.imp().is_dir.get());
        assert_eq!(a.imp().size.get(), 3);
        // 'fldr' whose name contains "UPLOAD" → the drop-box icon (421),
        // not the plain folder icon — the icon table's name heuristic.
        assert_eq!(a.imp().icon_id.get(), 421);

        let b = store.item(1).unwrap().downcast::<HxFileEntry>().unwrap();
        assert_eq!(b.imp().name.borrow().to_str().unwrap(), "readme.txt");
        assert!(!b.imp().is_dir.get());
        assert_eq!(b.imp().size.get(), 12);
    }

    #[test]
    fn clears_before_repopulating() {
        let store = make_store();
        fill(&store, &chunk(b"TEXT", 1, b"a.txt"));
        assert_eq!(store.n_items(), 1);

        // A second populate must replace, not append — exercise the FFI shell
        // which does the remove_all.
        let buf = chunk(b"fldr", 0, b"Docs");
        unsafe {
            gtkhx_files_populate_from_reply(store.as_ptr(), buf.as_ptr(), buf.len());
        }
        assert_eq!(store.n_items(), 1);
        let only = store.item(0).unwrap().downcast::<HxFileEntry>().unwrap();
        assert_eq!(only.imp().name.borrow().to_str().unwrap(), "Docs");
    }

    #[test]
    fn null_and_empty_just_clear() {
        let store = make_store();
        fill(&store, &chunk(b"TEXT", 1, b"a.txt"));
        assert_eq!(store.n_items(), 1);
        unsafe {
            gtkhx_files_populate_from_reply(store.as_ptr(), core::ptr::null(), 0);
        }
        assert_eq!(store.n_items(), 0);
        // NULL store is a no-op (must not crash).
        unsafe {
            gtkhx_files_populate_from_reply(core::ptr::null_mut(), core::ptr::null(), 0);
        }
    }

    #[test]
    fn unknown_fourcc_kind_is_labeled() {
        // "XYZ!" is not in the kind table → "XYZ! file" (untranslated in the
        // default catalog).
        let k = kind_for(*b"XYZ!");
        assert_eq!(k, "XYZ! file");
        // Non-printable bytes render as '?'.
        let k2 = kind_for([0x01, b'A', 0x7f, b'Z']);
        assert_eq!(k2, "?A?Z file");
    }
}
