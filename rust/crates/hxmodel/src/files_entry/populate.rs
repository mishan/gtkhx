//! Populate a files-browser `GListStore` from a folder's listing, as the
//! session read it: each entry's display name, its name as the server sent
//! it (what the browser names it back by), the dir flag, the icon id and the
//! kind label, built into an [`HxFileEntry`]. The store's contents are
//! replaced, so one call fully refreshes the listing.
//!
//! Everything here is pure Rust over `gio` (no GTK, no display), so it is
//! unit-tested headless.

use std::ffi::{c_char, CStr, CString};

use gio::prelude::ListModelExt;
use glib::translate::from_glib_none;

use crate::files_entry::HxFileEntry;
use hxsession::FileEntry;

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

/// Human kind label for a big-endian FourCC: a known type's English label run through the
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

/// Replace `store`'s contents with the listing's entries, in one `splice`.
///
/// One splice, not an append per entry: every change to the store emits
/// `items-changed`, and the Files panel answers each one with a sort-model
/// insert and a status-footer update. Row by row, a 10,000-entry folder
/// froze the UI for over a second (docs/performance.md).
fn fill(store: &gio::ListStore, files: &[FileEntry]) {
    let rows: Vec<HxFileEntry> = files
        .iter()
        .map(|f| {
            // The icon also reads the name as sent, for "DROP BOX" / "UPLOAD".
            let icon = crate::files::icon_id_for(Some(&f.type_code), Some(&f.name_bytes));
            let kind = kind_for(f.type_code);
            // No mtime on the wire (0).
            HxFileEntry::build(&f.name, f.folder, f.size, 0, &kind, icon).named_by(&f.name_bytes)
        })
        .collect();
    store.splice(0, store.n_items(), &rows);
}

/// Replace `store`'s contents with a listing's entries, as one change.
///
/// # Safety
/// `store` is a live `GListStore` of `HxFileEntry`.
pub unsafe fn populate(store: *mut std::ffi::c_void, files: &[FileEntry]) {
    let store: gio::ListStore = from_glib_none(store as *mut gio::ffi::GListStore);
    fill(&store, files);
}

#[cfg(test)]
mod tests {
    use super::*;
    // gio::prelude re-exports the glib prelude (StaticType / Cast), so it
    // covers the downcast + static_type used below.
    use gio::prelude::*;

    fn entry(type_code: &[u8; 4], size: u64, name: &[u8]) -> FileEntry {
        FileEntry {
            name: hxproto::text::to_utf8(name),
            name_bytes: name.to_vec(),
            folder: type_code == b"fldr",
            size,
            type_code: *type_code,
            creator: *b"MACR",
        }
    }

    fn make_store() -> gio::ListStore {
        gio::ListStore::with_type(HxFileEntry::static_type())
    }

    fn row(store: &gio::ListStore, i: u32) -> HxFileEntry {
        store.item(i).unwrap().downcast::<HxFileEntry>().unwrap()
    }

    #[test]
    fn populates_folder_and_file() {
        let store = make_store();
        fill(
            &store,
            &[
                entry(b"fldr", 3, b"Uploads"),
                entry(b"TEXT", 12, b"readme.txt"),
            ],
        );

        assert_eq!(store.n_items(), 2);

        let a = row(&store, 0);
        assert_eq!(a.name(), "Uploads");
        assert!(a.is_dir());
        assert_eq!(a.size(), 3);
        // 'fldr' whose name contains "UPLOAD" → the drop-box icon (421),
        // not the plain folder icon — the icon table's name heuristic.
        assert_eq!(a.icon_id(), 421);

        let b = row(&store, 1);
        assert_eq!(b.name(), "readme.txt");
        assert!(!b.is_dir());
        assert_eq!(b.size(), 12);
    }

    /// A Mac Roman name shows decoded, and is named back by its bytes.
    #[test]
    fn a_mac_roman_name_keeps_its_bytes() {
        let store = make_store();
        fill(&store, &[entry(b"TEXT", 1, b"caf\x8e")]);
        let e = row(&store, 0);
        assert_eq!(e.name(), "caf\u{e9}");
        assert_eq!(e.wire_name(), b"caf\x8e");
    }

    /// A whole listing lands as one `items-changed` that replaces the old
    /// rows, however many entries it holds. Per-row signals are what made a
    /// large folder freeze the Files panel.
    #[test]
    fn a_listing_is_one_change() {
        let store = make_store();
        fill(&store, &[entry(b"TEXT", 1, b"old.txt")]);
        let changes = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let seen = changes.clone();
        store.connect_items_changed(move |_, pos, removed, added| {
            seen.borrow_mut().push((pos, removed, added));
        });
        let files: Vec<FileEntry> = (0..500)
            .map(|i| entry(b"TEXT", i, format!("f{i}").as_bytes()))
            .collect();
        unsafe { populate(store.as_ptr().cast(), &files) };
        assert_eq!(*changes.borrow(), vec![(0, 1, 500)]);
        assert_eq!(store.n_items(), 500);
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
