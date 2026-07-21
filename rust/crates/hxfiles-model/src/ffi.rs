//! C-ABI surface for the files model. Symbols are `gtkhx_files_*` to match
//! the existing `gtkhx_proto_*` / `gtkhx_session_*` FFI naming.

use core::ffi::c_char;

/// Icon id for a Hotline file type + name — the FFI shim behind
/// `src/files.c::icon_of_ftype_and_name`.
///
/// # Safety
/// - `ftype`, when non-null, must point to at least 4 readable bytes (the
///   FourCC). A null pointer is treated as "unknown" and returns the
///   generic file icon.
/// - `name`, when non-null, must point to `name_len` readable bytes. It is
///   NOT required to be NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_icon_of_ftype_and_name(
    ftype: *const c_char,
    name: *const c_char,
    name_len: usize,
) -> u16 {
    let ftype_slice = if ftype.is_null() {
        None
    } else {
        // SAFETY: caller guarantees >= 4 readable bytes when non-null.
        Some(unsafe { core::slice::from_raw_parts(ftype as *const u8, 4) })
    };
    // Guard the slice length against the documented `from_raw_parts`
    // ceiling (isize::MAX): a length past it is instant UB. Real filenames
    // are tiny, so a pathological length (corrupt input, or a 32-bit host)
    // is treated as "no name" rather than risking UB.
    let name_slice = if name.is_null() || name_len == 0 || name_len > isize::MAX as usize {
        None
    } else {
        // SAFETY: non-null, and `name_len` (<= isize::MAX) readable bytes
        // per the caller contract.
        Some(unsafe { core::slice::from_raw_parts(name as *const u8, name_len) })
    };
    crate::icon_id_for(ftype_slice, name_slice)
}

/// Human label for a Hotline file type — the FFI shim behind the type
/// table in `src/files.c::kind_of_ftype`.
///
/// Returns a pointer to a static, NUL-terminated English label for a
/// known type, or NULL for an unknown/missing type (the C side then does
/// the `_()` translation, the "Unknown" null-type case, and the raw
/// FourCC fallback). The returned pointer is 'static — never freed.
///
/// # Safety
/// `ftype`, when non-null, must point to at least 4 readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_files_kind_label_for(ftype: *const c_char) -> *const c_char {
    let ftype_slice = if ftype.is_null() {
        None
    } else {
        // SAFETY: caller guarantees >= 4 readable bytes when non-null.
        Some(unsafe { core::slice::from_raw_parts(ftype as *const u8, 4) })
    };
    match crate::kind_label_for(ftype_slice) {
        Some(label) => label.as_ptr(),
        None => core::ptr::null(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::icon;

    #[test]
    fn ffi_matches_the_pure_fn() {
        unsafe {
            // null ftype
            assert_eq!(
                gtkhx_files_icon_of_ftype_and_name(core::ptr::null(), core::ptr::null(), 0),
                icon::FILE
            );
            // fldr + "Uploads"
            let ft = b"fldr";
            let nm = b"Uploads";
            assert_eq!(
                gtkhx_files_icon_of_ftype_and_name(
                    ft.as_ptr() as *const c_char,
                    nm.as_ptr() as *const c_char,
                    nm.len(),
                ),
                icon::FOLDER_IN
            );
            // JPEG, null name
            let jp = b"JPEG";
            assert_eq!(
                gtkhx_files_icon_of_ftype_and_name(
                    jp.as_ptr() as *const c_char,
                    core::ptr::null(),
                    0,
                ),
                icon::IMAGE
            );
        }
    }

    #[test]
    fn kind_ffi_returns_static_cstr_or_null() {
        unsafe {
            let mp3 = b"MP3 ";
            let p = gtkhx_files_kind_label_for(mp3.as_ptr() as *const c_char);
            assert!(!p.is_null());
            assert_eq!(core::ffi::CStr::from_ptr(p).to_str().unwrap(), "MP3 Audio");

            // unknown -> null
            let xx = b"XXXX";
            assert!(gtkhx_files_kind_label_for(xx.as_ptr() as *const c_char).is_null());
            // null ftype -> null
            assert!(gtkhx_files_kind_label_for(core::ptr::null()).is_null());
        }
    }
}
