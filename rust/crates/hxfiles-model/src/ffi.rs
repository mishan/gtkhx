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
    let name_slice = if name.is_null() || name_len == 0 {
        None
    } else {
        // SAFETY: caller guarantees `name_len` readable bytes when non-null.
        Some(unsafe { core::slice::from_raw_parts(name as *const u8, name_len) })
    };
    crate::icon_id_for(ftype_slice, name_slice)
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
}
