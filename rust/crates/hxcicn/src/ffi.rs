//! C-ABI shim for the `cicn` decoder.
//!
//! The crate is GTK-free (it produces RGBA), so the `GdkPixbuf` wrapping stays
//! in the thin C shim (`cicn.c`): it calls [`hxcicn_decode`], then hands the
//! returned `g_malloc`'d RGBA buffer to `gdk_pixbuf_new_from_data` with a
//! `g_free` destroy-notify (zero-copy). The halo is already applied.

use std::os::raw::c_uint;

/// Decode a `cicn` resource to a freshly `g_malloc`'d RGBA buffer.
///
/// Returns the buffer (`*out_w * *out_h * 4` bytes; caller frees with `g_free`,
/// e.g. via `gdk_pixbuf_new_from_data`'s destroy-notify) and writes the
/// dimensions through `out_w` / `out_h`. Returns NULL on malformed input.
///
/// # Safety
/// `rsrc` is valid for `len` bytes; `out_w` / `out_h` are writable `guint`s.
#[no_mangle]
pub unsafe extern "C" fn hxcicn_decode(
    rsrc: *const u8,
    len: usize,
    out_w: *mut c_uint,
    out_h: *mut c_uint,
) -> *mut u8 {
    if rsrc.is_null() || len == 0 {
        return std::ptr::null_mut();
    }
    let data = std::slice::from_raw_parts(rsrc, len);
    let Some(img) = crate::decode(data) else {
        return std::ptr::null_mut();
    };

    let n = img.pixels.len();
    let buf = glib::ffi::g_malloc(n) as *mut u8;
    if buf.is_null() {
        // g_malloc returned NULL (OOM) — copying into it would be UB.
        return std::ptr::null_mut();
    }
    std::ptr::copy_nonoverlapping(img.pixels.as_ptr(), buf, n);
    if !out_w.is_null() {
        *out_w = img.width;
    }
    if !out_h.is_null() {
        *out_h = img.height;
    }
    buf
}
