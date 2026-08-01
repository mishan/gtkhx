//! Headless `#[cfg(test)]` doubles for the C environment the receive handlers
//! reach through — the `hx_htxf_*` accessor seam, the genuine collaborators, and
//! the `gtkhx-core` emits. The setters record into a thread-local `FakeHtxf`;
//! the getters return configurable inputs; the outcome-bearing collaborators
//! (announce/start, error deletes, banner, file-info emit) set recorder flags.
//! `tests.rs` drives the handlers and asserts the recorded state.

use std::os::raw::{c_char, c_int, c_long, c_void};

pub(crate) mod test_env {
    use std::cell::{Cell, RefCell};

    /// The transfer state the setter doubles record.
    #[derive(Default, Clone)]
    pub struct FakeHtxf {
        pub ref_: u32,
        pub total_size: u64,
        pub queue: u32,
        pub data_pos: u64,
        pub rsrc_pos: u64,
        pub data_size: u64,
        pub rsrc_size: u64,
        pub gone: Option<u8>,
        pub preview: usize,
        pub serverhost: Vec<u8>,
        pub serverport: u16,
        pub start_stamped: bool,
    }

    thread_local! {
        pub static HTXF: RefCell<FakeHtxf> = RefCell::new(FakeHtxf::default());

        // Configurable getter inputs.
        pub static IN_LIST: Cell<i32> = const { Cell::new(1) };
        pub static OPT_RETRY: Cell<i32> = const { Cell::new(0) };
        pub static OPT_PREVIEW: Cell<i32> = const { Cell::new(0) };
        pub static PREVIEW_IN: Cell<usize> = const { Cell::new(0) };
        pub static DATA_SIZE_IN: Cell<u64> = const { Cell::new(0) };
        pub static STAT_SIZE: Cell<i64> = const { Cell::new(-1) };
        pub static RSRC_LEN: Cell<usize> = const { Cell::new(0) };
        pub static COMMENT_LEN: Cell<usize> = const { Cell::new(0) };

        // Outcome recorders.
        pub static ANNOUNCED: Cell<bool> = const { Cell::new(false) };
        pub static STARTED: Cell<bool> = const { Cell::new(false) };
        pub static XFER_DELETED: Cell<bool> = const { Cell::new(false) };
        pub static GTASK_DELETED: Cell<bool> = const { Cell::new(false) };
        pub static RETRY_TIMER: Cell<bool> = const { Cell::new(false) };
        pub static PREVIEW_BUILT: Cell<bool> = const { Cell::new(false) };
        pub static BANNER: Cell<Option<(u32, u32)>> = const { Cell::new(None) };
        pub static FILE_INFO: RefCell<Option<(Vec<u8>, u64)>> = const { RefCell::new(None) };
        /// The last pointer passed to g_free (the freed FILE_GETINFO label).
        pub static FREED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
    }

    pub fn reset() {
        FREED.with(|c| c.set(None));
        HTXF.with(|c| *c.borrow_mut() = FakeHtxf::default());
        IN_LIST.with(|c| c.set(1));
        OPT_RETRY.with(|c| c.set(0));
        OPT_PREVIEW.with(|c| c.set(0));
        PREVIEW_IN.with(|c| c.set(0));
        DATA_SIZE_IN.with(|c| c.set(0));
        STAT_SIZE.with(|c| c.set(-1));
        RSRC_LEN.with(|c| c.set(0));
        COMMENT_LEN.with(|c| c.set(0));
        ANNOUNCED.with(|c| c.set(false));
        STARTED.with(|c| c.set(false));
        XFER_DELETED.with(|c| c.set(false));
        GTASK_DELETED.with(|c| c.set(false));
        RETRY_TIMER.with(|c| c.set(false));
        PREVIEW_BUILT.with(|c| c.set(false));
        BANNER.with(|c| c.set(None));
        FILE_INFO.with(|c| *c.borrow_mut() = None);
    }
}

// A stable, always-valid C string the path getter can hand back. The fs-primitive
// doubles ignore its contents (they return configured values), so it never has to
// reflect a real file.
const FAKE_PATH: &[u8] = b"/upload.bin\0";
const FAKE_HOST: &[u8] = b"server.example\0";

// ---- gtkhx-core emits + session/kickoff ----

pub(crate) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

pub(crate) unsafe fn hx_sess_from_htlc(_htlc: *mut c_void) -> *mut c_void {
    std::ptr::null_mut()
}

pub(crate) unsafe fn gtkhx_session_emit_xfer_queue(
    _self_: *mut c_void,
    _sess: *mut c_void,
    _htxf: *mut c_void,
) {
    test_env::ANNOUNCED.with(|c| c.set(true));
}

pub(crate) unsafe fn xfer_ready_write(_htxf: *mut c_void) {
    test_env::STARTED.with(|c| c.set(true));
}

#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn gtkhx_session_emit_file_info(
    _self_: *mut c_void,
    _path: *const c_char,
    name: *const c_char,
    _creator: *const c_char,
    _type_: *const c_char,
    _comments: *const c_char,
    _date_modify: *const u8,
    _date_create: *const u8,
    size: u64,
) {
    let name = if name.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(name).to_bytes().to_vec()
    };
    test_env::FILE_INFO.with(|c| *c.borrow_mut() = Some((name, size)));
}

// ---- htxf accessor seam ----

pub(crate) unsafe fn hx_htxf_in_list(_htxf: *mut c_void) -> c_int {
    test_env::IN_LIST.with(|c| c.get())
}
pub(crate) unsafe fn hx_htxf_opt_retry(_htxf: *const c_void) -> c_int {
    test_env::OPT_RETRY.with(|c| c.get())
}
pub(crate) unsafe fn hx_htxf_opt_preview(_htxf: *const c_void) -> c_int {
    test_env::OPT_PREVIEW.with(|c| c.get())
}
pub(crate) unsafe fn hx_htxf_preview(_htxf: *const c_void) -> *mut c_void {
    test_env::PREVIEW_IN.with(|c| c.get()) as *mut c_void
}
pub(crate) unsafe fn hx_htxf_path(_htxf: *const c_void) -> *const c_char {
    FAKE_PATH.as_ptr() as *const c_char
}
pub(crate) unsafe fn hx_htxf_data_size(_htxf: *const c_void) -> u64 {
    test_env::DATA_SIZE_IN.with(|c| c.get())
}
pub(crate) unsafe fn hx_htxf_set_ref(_htxf: *mut c_void, ref_: u32) {
    test_env::HTXF.with(|c| c.borrow_mut().ref_ = ref_);
}
pub(crate) unsafe fn hx_htxf_set_total_size(_htxf: *mut c_void, total_size: u64) {
    test_env::HTXF.with(|c| c.borrow_mut().total_size = total_size);
}
pub(crate) unsafe fn hx_htxf_set_queue(_htxf: *mut c_void, queue: u32) {
    test_env::HTXF.with(|c| c.borrow_mut().queue = queue);
}
pub(crate) unsafe fn hx_htxf_set_data_pos(_htxf: *mut c_void, data_pos: u64) {
    test_env::HTXF.with(|c| c.borrow_mut().data_pos = data_pos);
}
pub(crate) unsafe fn hx_htxf_set_rsrc_pos(_htxf: *mut c_void, rsrc_pos: u64) {
    test_env::HTXF.with(|c| c.borrow_mut().rsrc_pos = rsrc_pos);
}
pub(crate) unsafe fn hx_htxf_set_data_size(_htxf: *mut c_void, data_size: u64) {
    test_env::HTXF.with(|c| c.borrow_mut().data_size = data_size);
}
pub(crate) unsafe fn hx_htxf_set_rsrc_size(_htxf: *mut c_void, rsrc_size: u64) {
    test_env::HTXF.with(|c| c.borrow_mut().rsrc_size = rsrc_size);
}
pub(crate) unsafe fn hx_htxf_set_gone(_htxf: *mut c_void, gone: u8) {
    test_env::HTXF.with(|c| c.borrow_mut().gone = Some(gone));
}
pub(crate) unsafe fn hx_htxf_set_preview(_htxf: *mut c_void, preview: *mut c_void) {
    test_env::HTXF.with(|c| c.borrow_mut().preview = preview as usize);
}
pub(crate) unsafe fn hx_htxf_set_serverhost(_htxf: *mut c_void, host: *const c_char) {
    let bytes = if host.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(host).to_bytes().to_vec()
    };
    test_env::HTXF.with(|c| c.borrow_mut().serverhost = bytes);
}
pub(crate) unsafe fn hx_htxf_set_serverport(_htxf: *mut c_void, port: u16) {
    test_env::HTXF.with(|c| c.borrow_mut().serverport = port);
}
pub(crate) unsafe fn hx_htxf_stamp_start(_htxf: *mut c_void) {
    test_env::HTXF.with(|c| c.borrow_mut().start_stamped = true);
}
pub(crate) unsafe fn hx_file_size(_path: *const c_char) -> i64 {
    test_env::STAT_SIZE.with(|c| c.get())
}

// ---- genuine collaborators ----

pub(crate) unsafe fn xfer_delete(_htxf: *mut c_void) {
    test_env::XFER_DELETED.with(|c| c.set(true));
}
pub(crate) unsafe fn gtask_delete_htxf(_sess: *mut c_void, _htxf: *mut c_void) {
    test_env::GTASK_DELETED.with(|c| c.set(true));
}
pub(crate) unsafe fn timer_add_secs(
    _secs: c_long,
    _f: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    _ptr: *mut c_void,
) {
    test_env::RETRY_TIMER.with(|c| c.set(true));
}
pub(crate) unsafe extern "C" fn xfer_go_timer(_arg: *mut c_void) -> c_int {
    0
}
pub(crate) unsafe fn hx_preview_new(_name: *const c_char) -> *mut c_void {
    test_env::PREVIEW_BUILT.with(|c| c.set(true));
    0xB0_usize as *mut c_void
}
pub(crate) unsafe fn hx_preview_set_cancel_cb(
    _p: *mut c_void,
    _f: Option<unsafe extern "C" fn(*mut c_void)>,
    _user_data: *mut c_void,
) {
}
pub(crate) unsafe fn dirchar_basename(path: *mut c_char) -> *mut c_char {
    path
}
pub(crate) unsafe fn resource_len(_path: *const c_char) -> usize {
    test_env::RSRC_LEN.with(|c| c.get())
}
pub(crate) unsafe fn comment_len(_path: *const c_char) -> usize {
    test_env::COMMENT_LEN.with(|c| c.get())
}
pub(crate) unsafe fn hx_conn_serverhost(_htlc: *const c_void) -> *const c_char {
    FAKE_HOST.as_ptr() as *const c_char
}
pub(crate) unsafe fn hx_conn_serverport(_htlc: *const c_void) -> u16 {
    5500
}
pub(crate) unsafe fn banner_handle_htxf_reply(_htlc: *mut c_void, ref_: u32, size: u32) {
    test_env::BANNER.with(|c| c.set(Some((ref_, size))));
}
pub(crate) unsafe fn g_free(ptr: *mut c_void) {
    test_env::FREED.with(|c| c.set(Some(ptr)));
}
