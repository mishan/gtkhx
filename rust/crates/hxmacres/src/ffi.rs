//! The `macres.h` C-ABI shim.
//!
//! Preserves the exact symbol names + `struct macres_res` layout so `macres.c`
//! can be deleted and the C consumers (`gtkhx.c` loads the `.rsrc` files,
//! `options.c` walks the icon picker, `cicn.c` looks up an icon by id) link this
//! crate instead. The `macres_file` handle is opaque (a boxed [`ResourceFork`]);
//! each `macres_res` the lookups return is `g_malloc`'d so a C caller can
//! `g_free` it (as `options.c` does), with its `data` pointer borrowing into the
//! handle's buffer (valid until `macres_file_delete`).

use std::ffi::{c_void, CStr};
use std::os::raw::c_char;

use crate::{Resource, ResourceFork};

/// `#[repr(C)]` mirror of `struct macres_res` (`macres.h`). `name`/`namelen`
/// are always NULL/0 (the C never populated them either).
#[repr(C)]
pub struct MacresRes {
    pub datalen: u32,
    pub resid: u16,
    pub namelen: u16,
    pub name: *mut u8,
    pub data: *mut c_void,
}

// Pin the layout the C header declares — both the overall size (pointer-width
// dependent: 24 on LP64, 16 on 32-bit) and every field offset, so a reorder /
// repacking / alignment regression fails to compile rather than silently
// breaking the macres.h contract.
const _: () = {
    use std::mem::{offset_of, size_of};
    let ptr = size_of::<*mut c_void>();
    assert!(size_of::<MacresRes>() == 8 + 2 * ptr);
    assert!(offset_of!(MacresRes, datalen) == 0);
    assert!(offset_of!(MacresRes, resid) == 4);
    assert!(offset_of!(MacresRes, namelen) == 6);
    assert!(offset_of!(MacresRes, name) == 8);
    assert!(offset_of!(MacresRes, data) == 8 + ptr);
};

/// Read all of the file at `cpath` into an owned `Vec`. Portable: no raw-fd /
/// POSIX handling, so it builds on every target. `None` on any read error.
fn read_path(cpath: &CStr) -> Option<Vec<u8>> {
    // GtkHx builds these paths with GLib (`g_build_filename`). On Unix, filenames
    // are opaque bytes (so we decode the raw bytes to preserve the exact path
    // semantics of open(2)); on non-Unix (Windows), GLib uses UTF-8, which std
    // maps to UTF-16 at the filesystem boundary.
    #[cfg(unix)]
    {
        use std::ffi::OsStr;
        use std::os::unix::ffi::OsStrExt;
        std::fs::read(OsStr::from_bytes(cpath.to_bytes())).ok()
    }
    #[cfg(not(unix))]
    {
        std::fs::read(cpath.to_str().ok()?).ok()
    }
}

/// `g_malloc` a `macres_res` wrapper around a borrowed [`Resource`]. The `data`
/// pointer aliases the handle's buffer (valid for the handle's lifetime).
unsafe fn alloc_res(res: Resource<'_>) -> *mut MacresRes {
    let p = glib::ffi::g_malloc(std::mem::size_of::<MacresRes>()) as *mut MacresRes;
    (*p).datalen = res.data.len() as u32;
    (*p).resid = res.resid as u16;
    (*p).namelen = 0;
    (*p).name = std::ptr::null_mut();
    (*p).data = res.data.as_ptr() as *mut c_void;
    p
}

/// `macres_file *macres_file_open (const char *path)` — read and parse the
/// resource fork at `path`. Returns an opaque handle, or NULL if the file
/// can't be read or isn't a valid resource fork.
///
/// # Safety
/// `path` is NULL or a valid NUL-terminated C string naming a readable file.
#[no_mangle]
pub unsafe extern "C" fn macres_file_open(path: *const c_char) -> *mut c_void {
    if path.is_null() {
        return std::ptr::null_mut();
    }
    let Some(data) = read_path(CStr::from_ptr(path)) else {
        return std::ptr::null_mut();
    };
    match ResourceFork::parse(data) {
        Some(rf) => Box::into_raw(Box::new(rf)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// `void macres_file_delete (macres_file *mrf)`
///
/// # Safety
/// `mrf` is NULL or a handle from `macres_file_open`, not used afterward.
#[no_mangle]
pub unsafe extern "C" fn macres_file_delete(mrf: *mut c_void) {
    if !mrf.is_null() {
        drop(Box::from_raw(mrf as *mut ResourceFork));
    }
}

/// `guint32 macres_file_num_res_of_type (macres_file *mrf, guint32 type)`
///
/// # Safety
/// `mrf` is NULL or a `macres_file_open` handle.
#[no_mangle]
pub unsafe extern "C" fn macres_file_num_res_of_type(mrf: *mut c_void, res_type: u32) -> u32 {
    match (mrf as *const ResourceFork).as_ref() {
        Some(rf) => rf.num_res_of_type(res_type),
        None => 0,
    }
}

/// `macres_res *macres_file_get_nth_res_of_type (macres_file *mrf, guint32 type,
/// guint32 n)` — a `g_malloc`'d wrapper (caller `g_free`s) or NULL.
///
/// # Safety
/// `mrf` is NULL or a `macres_file_open` handle.
#[no_mangle]
pub unsafe extern "C" fn macres_file_get_nth_res_of_type(
    mrf: *mut c_void,
    res_type: u32,
    n: u32,
) -> *mut MacresRes {
    match (mrf as *const ResourceFork).as_ref() {
        Some(rf) => match rf.nth_res_of_type(res_type, n) {
            Some(res) => alloc_res(res),
            None => std::ptr::null_mut(),
        },
        None => std::ptr::null_mut(),
    }
}

/// `macres_res *macres_file_get_resid_of_type (macres_file *mrf, guint32 type,
/// gint16 resid)` — a `g_malloc`'d wrapper (caller `g_free`s) or NULL.
///
/// # Safety
/// `mrf` is NULL or a `macres_file_open` handle.
#[no_mangle]
pub unsafe extern "C" fn macres_file_get_resid_of_type(
    mrf: *mut c_void,
    res_type: u32,
    resid: i16,
) -> *mut MacresRes {
    match (mrf as *const ResourceFork).as_ref() {
        Some(rf) => match rf.res_of_id(res_type, resid) {
            Some(res) => alloc_res(res),
            None => std::ptr::null_mut(),
        },
        None => std::ptr::null_mut(),
    }
}
