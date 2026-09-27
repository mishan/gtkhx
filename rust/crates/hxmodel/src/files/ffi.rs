//! C-ABI surface for the files model. Symbols are `gtkhx_files_*` to match
//! the existing `gtkhx_proto_*` / `gtkhx_session_*` FFI naming.

use core::ffi::c_char;

// ---- RemoteListing: the remote provider's path-navigation model ----------
//
// Opaque owned handle (same shape as hxproto's parse_dirlist /
// parse_catlist). The C `HxRemoteFilesProvider` holds one and delegates all
// path math + the sticky listing-error flag to it, keeping only the
// GListStore, the FILE_LIST RPC send, the no-reply watchdog, and the
// rcv-dispatch plumbing on the C side.

use crate::files::RemoteListing;

/// Create a fresh listing model rooted at `/`. The caller owns the handle
/// and must free it with [`gtkhx_files_listing_free`].
#[no_mangle]
pub extern "C" fn gtkhx_files_listing_new() -> *mut RemoteListing {
    Box::into_raw(Box::new(RemoteListing::new()))
}

/// Free a handle from [`gtkhx_files_listing_new`]. NULL is a no-op.
///
/// # Safety
/// `l` must be NULL or a pointer previously returned by
/// `gtkhx_files_listing_new`, freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_free(l: *mut RemoteListing) {
    if !l.is_null() {
        drop(Box::from_raw(l));
    }
}

/// Borrowed pointer to the current path (`/` at the root), valid until the
/// next mutation of this handle. Never NULL for a live handle.
///
/// # Safety
/// `l` must be NULL or a live handle. On NULL this returns NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_current_path(
    l: *const RemoteListing,
) -> *const c_char {
    if l.is_null() {
        return core::ptr::null();
    }
    (*l).current_c_ptr()
}

/// Adopt `path` as the current path. NULL or empty normalizes to `/`.
///
/// # Safety
/// `l` must be a live handle (NULL is a no-op). `path`, when non-null, must
/// be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_set_path(l: *mut RemoteListing, path: *const c_char) {
    if l.is_null() {
        return;
    }
    let p = if path.is_null() {
        String::new()
    } else {
        core::ffi::CStr::from_ptr(path)
            .to_string_lossy()
            .into_owned()
    };
    (*l).set_path(&p);
}

/// Return to the server root and clear the error flag.
///
/// # Safety
/// `l` must be NULL (no-op) or a live handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_reset(l: *mut RemoteListing) {
    if !l.is_null() {
        (*l).reset_to_root();
    }
}

/// TRUE iff the current path is the server root.
///
/// # Safety
/// `l` must be NULL (returns false) or a live handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_is_root(l: *const RemoteListing) -> bool {
    !l.is_null() && (*l).is_root()
}

/// The parent path to navigate to, as a freshly-allocated C string the
/// caller must release with [`gtkhx_files_string_free`]. NULL when already
/// at the root (or the path has no separator) — the provider's
/// `navigate_up` no-op case.
///
/// # Safety
/// `l` must be NULL (returns NULL) or a live handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_parent(l: *const RemoteListing) -> *mut c_char {
    if l.is_null() {
        return core::ptr::null_mut();
    }
    match (*l).parent() {
        Some(p) => string_into_raw(p),
        None => core::ptr::null_mut(),
    }
}

/// Build a server-side child path from the current path + `name`, as a
/// freshly-allocated C string the caller must release with
/// [`gtkhx_files_string_free`]. NULL only on NULL handle.
///
/// # Safety
/// `l` must be NULL (returns NULL) or a live handle. `name`, when non-null,
/// must be a valid NUL-terminated C string (NULL is treated as empty).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_child(
    l: *const RemoteListing,
    name: *const c_char,
) -> *mut c_char {
    if l.is_null() {
        return core::ptr::null_mut();
    }
    let n = if name.is_null() {
        String::new()
    } else {
        core::ffi::CStr::from_ptr(name)
            .to_string_lossy()
            .into_owned()
    };
    string_into_raw((*l).child(&n))
}

/// TRUE iff the most recent FILE_LIST failed (task error or no-reply
/// watchdog).
///
/// # Safety
/// `l` must be NULL (returns false) or a live handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_has_error(l: *const RemoteListing) -> bool {
    !l.is_null() && (*l).listing_error()
}

/// Set the sticky listing-error flag.
///
/// # Safety
/// `l` must be NULL (no-op) or a live handle.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_listing_set_error(l: *mut RemoteListing, v: bool) {
    if !l.is_null() {
        (*l).set_listing_error(v);
    }
}

/// Free a string returned by `gtkhx_files_listing_parent` /
/// `gtkhx_files_listing_child`. NULL is a no-op. Must NOT be used on
/// pointers from any other allocator (e.g. glib's `g_free`).
///
/// # Safety
/// `s` must be NULL or a pointer previously returned by one of those two
/// functions, freed exactly once.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(std::ffi::CString::from_raw(s));
    }
}

/// Allocate a C string from `s` for handoff to C. An interior NUL (not
/// possible from path math, defensive) collapses to an empty string.
fn string_into_raw(s: String) -> *mut c_char {
    std::ffi::CString::new(s)
        .unwrap_or_else(|_| std::ffi::CString::new("").unwrap())
        .into_raw()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn listing_ffi_roundtrip() {
        unsafe {
            let l = gtkhx_files_listing_new();
            assert!(!l.is_null());

            // starts at root
            let cur = gtkhx_files_listing_current_path(l);
            assert_eq!(core::ffi::CStr::from_ptr(cur).to_str().unwrap(), "/");
            assert!(gtkhx_files_listing_is_root(l));
            assert!(gtkhx_files_listing_parent(l).is_null());
            assert!(!gtkhx_files_listing_has_error(l));

            // child at root, then adopt it
            let uploads = std::ffi::CString::new("Uploads").unwrap();
            let child = gtkhx_files_listing_child(l, uploads.as_ptr());
            assert_eq!(
                core::ffi::CStr::from_ptr(child).to_str().unwrap(),
                "/Uploads"
            );
            gtkhx_files_listing_set_path(l, child);
            gtkhx_files_string_free(child);

            let cur = gtkhx_files_listing_current_path(l);
            assert_eq!(core::ffi::CStr::from_ptr(cur).to_str().unwrap(), "/Uploads");
            assert!(!gtkhx_files_listing_is_root(l));

            // parent walks back to root
            let par = gtkhx_files_listing_parent(l);
            assert_eq!(core::ffi::CStr::from_ptr(par).to_str().unwrap(), "/");
            gtkhx_files_string_free(par);

            // error flag + reset
            gtkhx_files_listing_set_error(l, true);
            assert!(gtkhx_files_listing_has_error(l));
            gtkhx_files_listing_reset(l);
            assert!(!gtkhx_files_listing_has_error(l));
            assert!(gtkhx_files_listing_is_root(l));

            // null-safety
            gtkhx_files_string_free(core::ptr::null_mut());
            gtkhx_files_listing_free(core::ptr::null_mut());
            assert!(gtkhx_files_listing_current_path(core::ptr::null()).is_null());

            gtkhx_files_listing_free(l);
        }
    }
}
