//! `gtkhx-ui` — Phase R5 window UIs in gtk4-rs.
//!
//! One module per ported window (`tracker` is the first). Each module
//! exports the C ABI entry points its deleted `src/<window>.c` used to
//! provide (via `#[no_mangle] pub extern "C"`), so the C callers
//! (toolbar.c, gtkhx.c signal adapters, options.c, network.c) link
//! against Rust with no source change. Shared infrastructure lives at
//! the crate root: [`ffi`] (extern decls for the C helpers windows
//! call), [`tr`] (gettext).
//!
//! Everything here runs on the GTK main thread; interior state is held in
//! `thread_local!`s. There is no `Send`/`Sync` surface — the windows
//! never touch worker threads directly (network / transfers already
//! marshal back to the main loop in `hxnet`/C before these entry points
//! fire).

mod ffi;
mod tr;

pub mod about;
pub mod agreement;
pub mod tracker;
pub mod useredit;

/// Tell gtk4-rs that GTK is already initialized.
///
/// The app initializes GTK from C (`gtk_init` / `adw_init`), so gtk4-rs's
/// own init flag was never set — and its widget/model constructors call
/// `assert_initialized_main_thread!()` on that flag, which panics (an
/// abort, since it can't unwind across the FFI) even though GTK is
/// running. Every gtk4-rs construction site reached from a C-ABI entry
/// point calls this first. Safe: `set_initialized()` verifies the real
/// `gtk_is_initialized()` and that we're on the main thread, and
/// early-returns once the binding is marked.
pub(crate) fn ensure_gtk_init() {
    unsafe { gtk4::set_initialized() };
}

/// C `char*` → owned `String` (empty on NULL). UTF-8 lossy.
///
/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
pub(crate) unsafe fn cstr(p: *const std::ffi::c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// `&str` → `CString`, dropping any interior NUL (never fails).
pub(crate) fn cs(s: &str) -> std::ffi::CString {
    std::ffi::CString::new(s).unwrap_or_else(|e| {
        let mut v = e.into_vec();
        v.retain(|&b| b != 0);
        std::ffi::CString::new(v).unwrap()
    })
}
