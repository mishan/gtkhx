//! Headless `#[cfg(test)]` doubles for the C environment `rcv_task_file_list`
//! reaches — the file-list emit, the provider error hook, and the recursive
//! engine. The `hx_cfl_*` accessors are the crate's own real functions, so tests
//! drive a real Rust-owned cfl and inspect it directly.

use std::os::raw::c_void;

pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    thread_local! {
        /// True after the file-list signal was emitted.
        pub static EMITTED: Cell<bool> = const { Cell::new(false) };
        /// True after the provider error hook fired.
        pub static PROVIDER_ERROR: Cell<bool> = const { Cell::new(false) };
        /// (is_folder, name-bytes, fsize) recorded per hx_cfl_complete_entry call.
        pub static COMPLETE_ENTRIES: RefCell<Vec<(bool, Vec<u8>, u32)>> =
            const { RefCell::new(Vec::new()) };
    }

    pub fn reset() {
        EMITTED.with(|c| c.set(false));
        PROVIDER_ERROR.with(|c| c.set(false));
        COMPLETE_ENTRIES.with(|c| c.borrow_mut().clear());
    }
}

pub(crate) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

pub(crate) unsafe fn gtkhx_session_emit_file_list(
    _self_: *mut c_void,
    _cfl: *mut c_void,
    _fh: *mut c_void,
    _data: *mut c_void,
) {
    test_env::EMITTED.with(|c| c.set(true));
}

pub(crate) unsafe fn hx_remote_files_provider_handle_file_list_error(
    _cfl: *mut c_void,
    _data: *mut c_void,
) -> std::os::raw::c_int {
    test_env::PROVIDER_ERROR.with(|c| c.set(true));
    1 // gboolean TRUE
}

pub(crate) unsafe fn hx_cfl_complete_entry(
    _htlc: *mut c_void,
    _cfl: *mut c_void,
    is_folder: std::os::raw::c_int,
    fname: *const u8,
    fnlen: usize,
    fsize: u32,
) {
    let name = if fname.is_null() {
        Vec::new()
    } else {
        std::slice::from_raw_parts(fname, fnlen).to_vec()
    };
    test_env::COMPLETE_ENTRIES.with(|c| c.borrow_mut().push((is_folder != 0, name, fsize)));
}
