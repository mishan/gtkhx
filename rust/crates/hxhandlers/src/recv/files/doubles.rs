//! Headless `#[cfg(test)]` doubles for the C environment `rcv_task_file_list`
//! reaches — the file-list emit and the provider error hook. The `hx_cfl_*` accessors are the crate's own real functions, so tests
//! drive a real Rust-owned cfl and inspect it directly.

use std::os::raw::c_void;

pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// True after the file-list signal was emitted.
        pub static EMITTED: Cell<bool> = const { Cell::new(false) };
        /// True after the provider error hook fired.
        pub static PROVIDER_ERROR: Cell<bool> = const { Cell::new(false) };
    }

    pub fn reset() {
        EMITTED.with(|c| c.set(false));
        PROVIDER_ERROR.with(|c| c.set(false));
    }
}

pub(crate) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

pub(crate) unsafe fn gtkhx_session_emit_file_list(
    _self_: *mut c_void,
    _htlc: *mut c_void,
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
