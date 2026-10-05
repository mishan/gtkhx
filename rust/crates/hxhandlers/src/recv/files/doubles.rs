//! Headless `#[cfg(test)]` doubles for the C environment `recv::files`
//! reaches — the file-list emit and the provider's error hook — recording
//! what each was handed.

use std::os::raw::c_void;

use super::{hx_cfl_path, CachedFileList};

pub(crate) mod test_env {
    use std::cell::RefCell;

    /// What reached a provider: the provider, the folder, and the names
    /// listed, or `None` for a refusal.
    pub type Reached = (usize, Vec<u8>, Option<Vec<Vec<u8>>>);

    thread_local! {
        pub static REACHED: RefCell<Vec<Reached>> = const { RefCell::new(Vec::new()) };
    }

    pub fn take() -> Vec<Reached> {
        REACHED.with(|r| std::mem::take(&mut *r.borrow_mut()))
    }
}

unsafe fn path(cfl: *mut c_void) -> Vec<u8> {
    std::ffi::CStr::from_ptr(hx_cfl_path(cfl as *const CachedFileList))
        .to_bytes()
        .to_vec()
}

pub(crate) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

pub(crate) unsafe fn gtkhx_session_emit_file_list(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cfl: *mut c_void,
    _fh: *mut c_void,
    data: *mut c_void,
) {
    let names = (*(cfl as *const CachedFileList))
        .files
        .iter()
        .map(|f| f.name_bytes.clone())
        .collect();
    let reached = (data as usize, path(cfl), Some(names));
    test_env::REACHED.with(|r| r.borrow_mut().push(reached));
}

pub(crate) unsafe fn hx_remote_files_provider_handle_file_list_error(
    cfl: *mut c_void,
    data: *mut c_void,
) -> std::os::raw::c_int {
    let reached = (data as usize, path(cfl), None);
    test_env::REACHED.with(|r| r.borrow_mut().push(reached));
    1
}

/// The tests' providers are not objects.
pub(crate) unsafe fn object_ref(_p: *mut c_void) {}
pub(crate) unsafe fn object_unref(_p: *mut c_void) {}
pub(crate) unsafe fn htxf_ref(_p: *mut c_void) {}
pub(crate) unsafe fn htxf_unref(_p: *mut c_void) {}
